#include "cache.h"
#include "delta.h"
#include "pack.h"
#include "csum-file.h"
#include "blob.h"
#include "commit.h"
#include "tag.h"
#include "tree.h"
#include "progress.h"

static const char index_pack_usage[] =
"git-index-pack [-v] [-o <index-file>] [{ ---keep | --keep=<msg> }] { <pack-file> | --stdin [--fix-thin] [<pack-file>] }";

struct object_entry
{
	struct pack_idx_entry idx;
	unsigned long size;
	unsigned int hdr_size;
	enum object_type type;
	enum object_type real_type;
};

union delta_base {
	unsigned char sha1[20];
	off_t offset;
};

/*
 * Even if sizeof(union delta_base) == 24 on 64-bit archs, we really want
 * to memcmp() only the first 20 bytes.
 */
#define UNION_BASE_SZ	20

struct delta_entry
{
	union delta_base base;
	int obj_no;
};

static struct object_entry *objects;
static struct delta_entry *deltas;
static int nr_objects;
static int nr_deltas;
static int nr_resolved_deltas;

static int from_stdin;
static int verbose;

static struct progress *progress;

/* We always read in 4kB chunks. */
static unsigned char input_buffer[4096];
static unsigned int input_offset, input_len;
static off_t consumed_bytes;
static SHA_CTX input_ctx;
static uint32_t input_crc32;
static int input_fd, output_fd, pack_fd;

/* Discard current buffer used content. */
static void flush(void)
{
	if (input_offset) {
		if (output_fd >= 0)
			write_or_die(output_fd, input_buffer, input_offset);
		SHA1_Update(&input_ctx, input_buffer, input_offset);
		memmove(input_buffer, input_buffer + input_offset, input_len);
		input_offset = 0;
	}
}

/*
 * Make sure at least "min" bytes are available in the buffer, and
 * return the pointer to the buffer.
 */
static void *fill(int min)
{
	if (min <= input_len)
		return input_buffer + input_offset;
	if (min > sizeof(input_buffer))
		die("cannot fill %d bytes", min);
	flush();
	do {
		ssize_t ret = xread(input_fd, input_buffer + input_len,
				sizeof(input_buffer) - input_len);
		if (ret <= 0) {
			if (!ret)
				die("early EOF");
			die("read error on input: %s", strerror(errno));
		}
		if (from_stdin)
			display_throughput(progress, ret);
		input_len += ret;
	} while (input_len < min);
	return input_buffer;
}

static void use(int bytes)
{
	if (bytes > input_len)
		die("used more bytes than were available");
	input_crc32 = crc32(input_crc32, input_buffer + input_offset, bytes);
	input_len -= bytes;
	input_offset += bytes;

	/* make sure off_t is sufficiently large not to wrap */
	if (consumed_bytes > consumed_bytes + bytes)
		die("pack too large for current definition of off_t");
	consumed_bytes += bytes;
}

static char *open_pack_file(char *pack_name)
{
	if (from_stdin) {
		input_fd = 0;
		if (!pack_name) {
			static char tmpfile[PATH_MAX];
			snprintf(tmpfile, sizeof(tmpfile),
				 "%s/tmp_pack_XXXXXX", get_object_directory());
			output_fd = xmkstemp(tmpfile);
			pack_name = xstrdup(tmpfile);
		} else
			output_fd = open(pack_name, O_CREAT|O_EXCL|O_RDWR, 0600);
		if (output_fd < 0)
			die("unable to create %s: %s\n", pack_name, strerror(errno));
		pack_fd = output_fd;
	} else {
		input_fd = open(pack_name, O_RDONLY);
		if (input_fd < 0)
			die("cannot open packfile '%s': %s",
			    pack_name, strerror(errno));
		output_fd = -1;
		pack_fd = input_fd;
	}
	SHA1_Init(&input_ctx);
	return pack_name;
}

static void parse_pack_header(void)
{
	struct pack_header *hdr = fill(sizeof(struct pack_header));

	/* Header consistency check */
	if (hdr->hdr_signature != htonl(PACK_SIGNATURE))
		die("pack signature mismatch");
	if (!pack_version_ok(hdr->hdr_version))
		die("pack version %d unsupported", ntohl(hdr->hdr_version));

	nr_objects = ntohl(hdr->hdr_entries);
	use(sizeof(struct pack_header));
}

static void bad_object(unsigned long offset, const char *format,
		       ...) NORETURN __attribute__((format (printf, 2, 3)));

static void bad_object(unsigned long offset, const char *format, ...)
{
	va_list params;
	char buf[1024];

	va_start(params, format);
	vsnprintf(buf, sizeof(buf), format, params);
	va_end(params);
	die("pack has bad object at offset %lu: %s", offset, buf);
}

static void *unpack_entry_data(unsigned long offset, unsigned long size)
{
	z_stream stream;
	void *buf = xmalloc(size);

	memset(&stream, 0, sizeof(stream));
	stream.next_out = buf;
	stream.avail_out = size;
	stream.next_in = fill(1);
	stream.avail_in = input_len;
	inflateInit(&stream);

	for (;;) {
		int ret = inflate(&stream, 0);
		use(input_len - stream.avail_in);
		if (stream.total_out == size && ret == Z_STREAM_END)
			break;
		if (ret != Z_OK)
			bad_object(offset, "inflate returned %d", ret);
		stream.next_in = fill(1);
		stream.avail_in = input_len;
	}
	inflateEnd(&stream);
	return buf;
}

static void *unpack_raw_entry(struct object_entry *obj, union delta_base *delta_base)
{
	unsigned char *p, c;
	unsigned long size;
	off_t base_offset;
	unsigned shift;
	void *data;

	obj->idx.offset = consumed_bytes;
	input_crc32 = crc32(0, Z_NULL, 0);

	p = fill(1);
	c = *p;
	use(1);
	obj->type = (c >> 4) & 7;
	size = (c & 15);
	shift = 4;
	while (c & 0x80) {
		p = fill(1);
		c = *p;
		use(1);
		size += (c & 0x7fUL) << shift;
		shift += 7;
	}
	obj->size = size;

	switch (obj->type) {
	case OBJ_REF_DELTA:
		hashcpy(delta_base->sha1, fill(20));
		use(20);
		break;
	case OBJ_OFS_DELTA:
		memset(delta_base, 0, sizeof(*delta_base));
		p = fill(1);
		c = *p;
		use(1);
		base_offset = c & 127;
		while (c & 128) {
			base_offset += 1;
			if (!base_offset || MSB(base_offset, 7))
				bad_object(obj->idx.offset, "offset value overflow for delta base object");
			p = fill(1);
			c = *p;
			use(1);
			base_offset = (base_offset << 7) + (c & 127);
		}
		delta_base->offset = obj->idx.offset - base_offset;
		if (delta_base->offset >= obj->idx.offset)
			bad_object(obj->idx.offset, "delta base offset is out of bound");
		break;
	case OBJ_COMMIT:
	case OBJ_TREE:
	case OBJ_BLOB:
	case OBJ_TAG:
		break;
	default:
		bad_object(obj->idx.offset, "unknown object type %d", obj->type);
	}
	obj->hdr_size = consumed_bytes - obj->idx.offset;

	data = unpack_entry_data(obj->idx.offset, obj->size);
	obj->idx.crc32 = input_crc32;
	return data;
}

static void *get_data_from_pack(struct object_entry *obj)
{
	unsigned long from = obj[0].idx.offset + obj[0].hdr_size;
	unsigned long len = obj[1].idx.offset - from;
	unsigned long rdy = 0;
	unsigned char *src, *data;
	z_stream stream;
	int st;

	src = xmalloc(len);
	data = src;
	do {
		ssize_t n = pread(pack_fd, data + rdy, len - rdy, from + rdy);
		if (n <= 0)
			die("cannot pread pack file: %s", strerror(errno));
		rdy += n;
	} while (rdy < len);
	data = xmalloc(obj->size);
	memset(&stream, 0, sizeof(stream));
	stream.next_out = data;
	stream.avail_out = obj->size;
	stream.next_in = src;
	stream.avail_in = len;
	inflateInit(&stream);
	while ((st = inflate(&stream, Z_FINISH)) == Z_OK);
	inflateEnd(&stream);
	if (st != Z_STREAM_END || stream.total_out != obj->size)
		die("serious inflate inconsistency");
	free(src);
	return data;
}

static int find_delta(const union delta_base *base)
{
	int first = 0, last = nr_deltas;

        while (first < last) {
                int next = (first + last) / 2;
                struct delta_entry *delta = &deltas[next];
                int cmp;

                cmp = memcmp(base, &delta->base, UNION_BASE_SZ);
                if (!cmp)
                        return next;
                if (cmp < 0) {
                        last = next;
                        continue;
                }
                first = next+1;
        }
        return -first-1;
}

static int find_delta_children(const union delta_base *base,
			       int *first_index, int *last_index)
{
	int first = find_delta(base);
	int last = first;
	int end = nr_deltas - 1;

	if (first < 0)
		return -1;
	while (first > 0 && !memcmp(&deltas[first - 1].base, base, UNION_BASE_SZ))
		--first;
	while (last < end && !memcmp(&deltas[last + 1].base, base, UNION_BASE_SZ))
		++last;
	*first_index = first;
	*last_index = last;
	return 0;
}

static void sha1_object(const void *data, unsigned long size,
			enum object_type type, unsigned char *sha1)
{
	hash_sha1_file(data, size, typename(type), sha1);
	if (has_sha1_file(sha1)) {
		void *has_data;
		enum object_type has_type;
		unsigned long has_size;
		has_data = read_sha1_file(sha1, &has_type, &has_size);
		if (!has_data)
			die("cannot read existing object %s", sha1_to_hex(sha1));
		if (size != has_size || type != has_type ||
		    memcmp(data, has_data, size) != 0)
			die("SHA1 COLLISION FOUND WITH %s !", sha1_to_hex(sha1));
		free(has_data);
	}
}

static void resolve_delta(struct object_entry *delta_obj, void *base_data,
			  unsigned long base_size, enum object_type type)
{
	void *delta_data;
	unsigned long delta_size;
	void *result;
	unsigned long result_size;
	union delta_base delta_base;
	int j, first, last;

	delta_obj->real_type = type;
	delta_data = get_data_from_pack(delta_obj);
	delta_size = delta_obj->size;
	result = patch_delta(base_data, base_size, delta_data, delta_size,
			     &result_size);
	free(delta_data);
	if (!result)
		bad_object(delta_obj->idx.offset, "failed to apply delta");
	sha1_object(result, result_size, type, delta_obj->idx.sha1);
	nr_resolved_deltas++;

	hashcpy(delta_base.sha1, delta_obj->idx.sha1);
	if (!find_delta_children(&delta_base, &first, &last)) {
		for (j = first; j <= last; j++) {
			struct object_entry *child = objects + deltas[j].obj_no;
			if (child->real_type == OBJ_REF_DELTA)
				resolve_delta(child, result, result_size, type);
		}
	}

	memset(&delta_base, 0, sizeof(delta_base));
	delta_base.offset = delta_obj->idx.offset;
	if (!find_delta_children(&delta_base, &first, &last)) {
		for (j = first; j <= last; j++) {
			struct object_entry *child = objects + deltas[j].obj_no;
			if (child->real_type == OBJ_OFS_DELTA)
				resolve_delta(child, result, result_size, type);
		}
	}

	free(result);
}

static int compare_delta_entry(const void *a, const void *b)
{
	const struct delta_entry *delta_a = a;
	const struct delta_entry *delta_b = b;
	return memcmp(&delta_a->base, &delta_b->base, UNION_BASE_SZ);
}

/* Parse all objects and return the pack content SHA1 hash */
static void parse_pack_objects(unsigned char *sha1)
{
	int i;
	struct delta_entry *delta = deltas;
	void *data;
	struct stat st;

	/*
	 * First pass:
	 * - find locations of all objects;
	 * - calculate SHA1 of all non-delta objects;
	 * - remember base (SHA1 or offset) for all deltas.
	 */
	if (verbose)
		progress = start_progress(
				from_stdin ? "Receiving objects" : "Indexing objects",
				nr_objects);
	for (i = 0; i < nr_objects; i++) {
		struct object_entry *obj = &objects[i];
		data = unpack_raw_entry(obj, &delta->base);
		obj->real_type = obj->type;
		if (obj->type == OBJ_REF_DELTA || obj->type == OBJ_OFS_DELTA) {
			nr_deltas++;
			delta->obj_no = i;
			delta++;
		} else
			sha1_object(data, obj->size, obj->type, obj->idx.sha1);
		free(data);
		display_progress(progress, i+1);
	}
	objects[i].idx.offset = consumed_bytes;
	stop_progress(&progress);

	/* Check pack integrity */
	flush();
	SHA1_Final(sha1, &input_ctx);
	if (hashcmp(fill(20), sha1))
		die("pack is corrupted (SHA1 mismatch)");
	use(20);

	/* If input_fd is a file, we should have reached its end now. */
	if (fstat(input_fd, &st))
		die("cannot fstat packfile: %s", strerror(errno));
	if (S_ISREG(st.st_mode) &&
			lseek(input_fd, 0, SEEK_CUR) - input_len != st.st_size)
		die("pack has junk at the end");

	if (!nr_deltas)
		return;

	/* Sort deltas by base SHA1/offset for fast searching */
	qsort(deltas, nr_deltas, sizeof(struct delta_entry),
	      compare_delta_entry);

	/*
	 * Second pass:
	 * - for all non-delta objects, look if it is used as a base for
	 *   deltas;
	 * - if used as a base, uncompress the object and apply all deltas,
	 *   recursively checking if the resulting object is used as a base
	 *   for some more deltas.
	 */
	if (verbose)
		progress = start_progress("Resolving deltas", nr_deltas);
	for (i = 0; i < nr_objects; i++) {
		struct object_entry *obj = &objects[i];
		union delta_base base;
		int j, ref, ref_first, ref_last, ofs, ofs_first, ofs_last;

		if (obj->type == OBJ_REF_DELTA || obj->type == OBJ_OFS_DELTA)
			continue;
		hashcpy(base.sha1, obj->idx.sha1);
		ref = !find_delta_children(&base, &ref_first, &ref_last);
		memset(&base, 0, sizeof(base));
		base.offset = obj->idx.offset;
		ofs = !find_delta_children(&base, &ofs_first, &ofs_last);
		if (!ref && !ofs)
			continue;
		data = get_data_from_pack(obj);
		if (ref)
			for (j = ref_first; j <= ref_last; j++) {
				struct object_entry *child = objects + deltas[j].obj_no;
				if (child->real_type == OBJ_REF_DELTA)
					resolve_delta(child, data,
						      obj->size, obj->type);
			}
		if (ofs)
			for (j = ofs_first; j <= ofs_last; j++) {
				struct object_entry *child = objects + deltas[j].obj_no;
				if (child->real_type == OBJ_OFS_DELTA)
					resolve_delta(child, data,
						      obj->size, obj->type);
			}
		free(data);
		display_progress(progress, nr_resolved_deltas);
	}
}

static int write_compressed(int fd, void *in, unsigned int size, uint32_t *obj_crc)
{
	z_stream stream;
	unsigned long maxsize;
	void *out;

	memset(&stream, 0, sizeof(stream));
	deflateInit(&stream, zlib_compression_level);
	maxsize = deflateBound(&stream, size);
	out = xmalloc(maxsize);

	/* Compress it */
	stream.next_in = in;
	stream.avail_in = size;
	stream.next_out = out;
	stream.avail_out = maxsize;
	while (deflate(&stream, Z_FINISH) == Z_OK);
	deflateEnd(&stream);

	size = stream.total_out;
	write_or_die(fd, out, size);
	*obj_crc = crc32(*obj_crc, out, size);
	free(out);
	return size;
}

static void append_obj_to_pack(const unsigned char *sha1, void *buf,
			       unsigned long size, enum object_type type)
{
	struct object_entry *obj = &objects[nr_objects++];
	unsigned char header[10];
	unsigned long s = size;
	int n = 0;
	unsigned char c = (type << 4) | (s & 15);
	s >>= 4;
	while (s) {
		header[n++] = c | 0x80;
		c = s & 0x7f;
		s >>= 7;
	}
	header[n++] = c;
	write_or_die(output_fd, header, n);
	obj[0].idx.crc32 = crc32(0, Z_NULL, 0);
	obj[0].idx.crc32 = crc32(obj[0].idx.crc32, header, n);
	obj[1].idx.offset = obj[0].idx.offset + n;
	obj[1].idx.offset += write_compressed(output_fd, buf, size, &obj[0].idx.crc32);
	hashcpy(obj->idx.sha1, sha1);
}

static int delta_pos_compare(const void *_a, const void *_b)
{
	struct delta_entry *a = *(struct delta_entry **)_a;
	struct delta_entry *b = *(struct delta_entry **)_b;
	return a->obj_no - b->obj_no;
}

static void fix_unresolved_deltas(int nr_unresolved)
{
	struct delta_entry **sorted_by_pos;
	int i, n = 0;

	/*
	 * Since many unresolved deltas may well be themselves base objects
	 * for more unresolved deltas, we really want to include the
	 * smallest number of base objects that would cover as much delta
	 * as possible by picking the
	 * trunc deltas first, allowing for other deltas to resolve without
	 * additional base objects.  Since most base objects are to be found
	 * before deltas depending on them, a good heuristic is to start
	 * resolving deltas in the same order as their position in the pack.
	 */
	sorted_by_pos = xmalloc(nr_unresolved * sizeof(*sorted_by_pos));
	for (i = 0; i < nr_deltas; i++) {
		if (objects[deltas[i].obj_no].real_type != OBJ_REF_DELTA)
			continue;
		sorted_by_pos[n++] = &deltas[i];
	}
	qsort(sorted_by_pos, n, sizeof(*sorted_by_pos), delta_pos_compare);

	for (i = 0; i < n; i++) {
		struct delta_entry *d = sorted_by_pos[i];
		void *data;
		unsigned long size;
		enum object_type type;
		int j, first, last;

		if (objects[d->obj_no].real_type != OBJ_REF_DELTA)
			continue;
		data = read_sha1_file(d->base.sha1, &type, &size);
		if (!data)
			continue;

		find_delta_children(&d->base, &first, &last);
		for (j = first; j <= last; j++) {
			struct object_entry *child = objects + deltas[j].obj_no;
			if (child->real_type == OBJ_REF_DELTA)
				resolve_delta(child, data, size, type);
		}

		if (check_sha1_signature(d->base.sha1, data, size, typename(type)))
			die("local object %s is corrupt", sha1_to_hex(d->base.sha1));
		append_obj_to_pack(d->base.sha1, data, size, type);
		free(data);
		display_progress(progress, nr_resolved_deltas);
	}
	free(sorted_by_pos);
}

static void final(const char *final_pack_name, const char *curr_pack_name,
		  const char *final_index_name, const char *curr_index_name,
		  const char *keep_name, const char *keep_msg,
		  unsigned char *sha1)
{
	const char *report = "pack";
	char name[PATH_MAX];
	int err;

	if (!from_stdin) {
		close(input_fd);
	} else {
		err = close(output_fd);
		if (err)
			die("error while closing pack file: %s", strerror(errno));
		chmod(curr_pack_name, 0444);
	}

	if (keep_msg) {
		int keep_fd, keep_msg_len = strlen(keep_msg);
		if (!keep_name) {
			snprintf(name, sizeof(name), "%s/pack/pack-%s.keep",
				 get_object_directory(), sha1_to_hex(sha1));
			keep_name = name;
		}
		keep_fd = open(keep_name, O_RDWR|O_CREAT|O_EXCL, 0600);
		if (keep_fd < 0) {
			if (errno != EEXIST)
				die("cannot write keep file");
		} else {
			if (keep_msg_len > 0) {
				write_or_die(keep_fd, keep_msg, keep_msg_len);
				write_or_die(keep_fd, "\n", 1);
			}
			if (close(keep_fd) != 0)
				die("cannot write keep file");
			report = "keep";
		}
	}

	if (final_pack_name != curr_pack_name) {
		if (!final_pack_name) {
			snprintf(name, sizeof(name), "%s/pack/pack-%s.pack",
				 get_object_directory(), sha1_to_hex(sha1));
			final_pack_name = name;
		}
		if (move_temp_to_file(curr_pack_name, final_pack_name))
			die("cannot store pack file");
	}

	chmod(curr_index_name, 0444);
	if (final_index_name != curr_index_name) {
		if (!final_index_name) {
			snprintf(name, sizeof(name), "%s/pack/pack-%s.idx",
				 get_object_directory(), sha1_to_hex(sha1));
			final_index_name = name;
		}
		if (move_temp_to_file(curr_index_name, final_index_name))
			die("cannot store index file");
	}

	if (!from_stdin) {
		printf("%s\n", sha1_to_hex(sha1));
	} else {
		char buf[48];
		int len = snprintf(buf, sizeof(buf), "%s\t%s\n",
				   report, sha1_to_hex(sha1));
		write_or_die(1, buf, len);

		/*
		 * Let's just mimic git-unpack-objects here and write
		 * the last part of the input buffer to stdout.
		 */
		while (input_len) {
			err = xwrite(1, input_buffer + input_offset, input_len);
			if (err <= 0)
				break;
			input_len -= err;
			input_offset += err;
		}
	}
}

int main(int argc, char **argv)
{
	int i, fix_thin_pack = 0;
	char *curr_pack, *pack_name = NULL;
	char *curr_index, *index_name = NULL;
	const char *keep_name = NULL, *keep_msg = NULL;
	char *index_name_buf = NULL, *keep_name_buf = NULL;
	struct pack_idx_entry **idx_objects;
	unsigned char sha1[20];

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];

		if (*arg == '-') {
			if (!strcmp(arg, "--stdin")) {
				from_stdin = 1;
			} else if (!strcmp(arg, "--fix-thin")) {
				fix_thin_pack = 1;
			} else if (!strcmp(arg, "--keep")) {
				keep_msg = "";
			} else if (!prefixcmp(arg, "--keep=")) {
				keep_msg = arg + 7;
			} else if (!prefixcmp(arg, "--pack_header=")) {
				struct pack_header *hdr;
				char *c;

				hdr = (struct pack_header *)input_buffer;
				hdr->hdr_signature = htonl(PACK_SIGNATURE);
				hdr->hdr_version = htonl(strtoul(arg + 14, &c, 10));
				if (*c != ',')
					die("bad %s", arg);
				hdr->hdr_entries = htonl(strtoul(c + 1, &c, 10));
				if (*c)
					die("bad %s", arg);
				input_len = sizeof(*hdr);
			} else if (!strcmp(arg, "-v")) {
				verbose = 1;
			} else if (!strcmp(arg, "-o")) {
				if (index_name || (i+1) >= argc)
					usage(index_pack_usage);
				index_name = argv[++i];
			} else if (!prefixcmp(arg, "--index-version=")) {
				char *c;
				pack_idx_default_version = strtoul(arg + 16, &c, 10);
				if (pack_idx_default_version > 2)
					die("bad %s", arg);
				if (*c == ',')
					pack_idx_off32_limit = strtoul(c+1, &c, 0);
				if (*c || pack_idx_off32_limit & 0x80000000)
					die("bad %s", arg);
			} else
				usage(index_pack_usage);
			continue;
		}

		if (pack_name)
			usage(index_pack_usage);
		pack_name = arg;
	}

	if (!pack_name && !from_stdin)
		usage(index_pack_usage);
	if (fix_thin_pack && !from_stdin)
		die("--fix-thin cannot be used without --stdin");
	if (!index_name && pack_name) {
		int len = strlen(pack_name);
		if (!has_extension(pack_name, ".pack"))
			die("packfile name '%s' does not end with '.pack'",
			    pack_name);
		index_name_buf = xmalloc(len);
		memcpy(index_name_buf, pack_name, len - 5);
		strcpy(index_name_buf + len - 5, ".idx");
		index_name = index_name_buf;
	}
	if (keep_msg && !keep_name && pack_name) {
		int len = strlen(pack_name);
		if (!has_extension(pack_name, ".pack"))
			die("packfile name '%s' does not end with '.pack'",
			    pack_name);
		keep_name_buf = xmalloc(len);
		memcpy(keep_name_buf, pack_name, len - 5);
		strcpy(keep_name_buf + len - 5, ".keep");
		keep_name = keep_name_buf;
	}

	curr_pack = open_pack_file(pack_name);
	parse_pack_header();
	objects = xmalloc((nr_objects + 1) * sizeof(struct object_entry));
	deltas = xmalloc(nr_objects * sizeof(struct delta_entry));
	parse_pack_objects(sha1);
	if (nr_deltas == nr_resolved_deltas) {
		stop_progress(&progress);
		/* Flush remaining pack final 20-byte SHA1. */
		flush();
	} else {
		if (fix_thin_pack) {
			int nr_unresolved = nr_deltas - nr_resolved_deltas;
			int nr_objects_initial = nr_objects;
			if (nr_unresolved <= 0)
				die("confusion beyond insanity");
			objects = xrealloc(objects,
					   (nr_objects + nr_unresolved + 1)
					   * sizeof(*objects));
			fix_unresolved_deltas(nr_unresolved);
			stop_progress(&progress);
			if (verbose)
				fprintf(stderr, "%d objects were added to complete this thin pack.\n",
					nr_objects - nr_objects_initial);
			fixup_pack_header_footer(output_fd, sha1,
				curr_pack, nr_objects);
		}
		if (nr_deltas != nr_resolved_deltas)
			die("pack has %d unresolved deltas",
			    nr_deltas - nr_resolved_deltas);
	}
	free(deltas);

	idx_objects = xmalloc((nr_objects) * sizeof(struct pack_idx_entry *));
	for (i = 0; i < nr_objects; i++)
		idx_objects[i] = &objects[i].idx;
	curr_index = write_idx_file(index_name, idx_objects, nr_objects, sha1);
	free(idx_objects);

	final(pack_name, curr_pack,
		index_name, curr_index,
		keep_name, keep_msg,
		sha1);
	free(objects);
	free(index_name_buf);
	free(keep_name_buf);
	if (pack_name == NULL)
		free(curr_pack);
	if (index_name == NULL)
		free(curr_index);

	return 0;
}
