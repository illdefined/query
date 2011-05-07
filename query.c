#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <buffer.h>
#include <fmt.h>
#include <scan.h>
#include <str.h>
#include <stralloc.h>

#include <sqlite3.h>

struct param {
	sqlite3_stmt *stmt;
	int index;
};

struct token {
	char *string;
	size_t length;
};

static wchar_t radix = L'.';
static wchar_t psign = L'+';
static wchar_t nsign = L'-';

static sqlite3 *db;

static void final() {
	sqlite3_close(db);
}

static void die(const char *restrict mesg, ...) {
	va_list ap;

	va_start(ap, mesg);
	do {
		buffer_puts(buffer_2, mesg);
		mesg = va_arg(ap, const char *);
	} while (mesg);
	va_end(ap);

	buffer_putnlflush(buffer_2);
	exit(EXIT_FAILURE);
}

static size_t extractWide(wchar_t *restrict wide,
	const char *restrict string, size_t length) {
	size_t width = mbrtowc(wide, string, length, NULL);
	switch (width) {
	case (size_t) -2:
		return 0;
	case (size_t) -1:
		die("Invalid multi‐byte character in input", NULL);
	case 0:
		return 1;
	default:
		return width;
	}
}

static void cram(wchar_t *restrict wide,
	const char *restrict string) {
	if (*string)
		extractWide(wide, string, strlen(string));
}

static void locale() {
	setlocale(LC_ALL, "");

	struct lconv *lconv = localeconv();

	cram(&radix, lconv->decimal_point);
	cram(&psign, lconv->positive_sign);
	cram(&nsign, lconv->negative_sign);
}

static uint8_t scanNibble(char nibble) {
	int ret = scan_fromhex(nibble);
	if (ret < 0)
		die("Invalid nibble in blob", NULL);
	return ret;
}

static uint8_t scanOctet(const char octet[2]) {
	return
		  scanNibble(octet[0]) << 4
		| scanNibble(octet[1]);
}

static void bindQuoted(const struct param *restrict param,
	struct token *restrict token) {

	if (token->length < 2
		|| token->string[token->length - 1] != '\'')
		die("No single quotation mark at end of text", NULL);

	/* Clip quotation marks */
	char *string = token->string + 1;
	size_t length = token->length - 2;

	bool quote = false;
	size_t head = 0;
	size_t tail = 0;

	while (tail < length) {
		wchar_t wide;
		size_t width = extractWide(&wide, string + tail, length - tail);
		if (width == 0)
			die("Incomplete multi‐byte character inside quoted text", NULL);

		if (quote) {
			if (wide == L'\'')
				quote = false;
			else
				die("Unescaped quotation mark inside quoted text", NULL);
		}
		else {
			if (wide == L'\'')
				quote = true;

			memmove(string + head, string + tail, width);
			head += width;
		}

		tail += width;
	}

	if (quote)
		die("Unescaped quotation mark inside quoted text", NULL);

	if (sqlite3_bind_text(param->stmt, param->index,
		string, head, SQLITE_STATIC))
		die("Failed to bind text: ",
			sqlite3_errmsg(db), NULL);
}

static void bindBlob(const struct param *restrict param,
	struct token *restrict token) {

	if (token->length % 2)
		die("Invalid blob token length", NULL);
	if (token->length < 2 ||
		token->string[token->length - 1] != '}')
		die("No closing brace at end of blob", NULL);

	/* Clip braces and get blob length */
	char *string = token->string + 1;
	size_t binlen = token->length / 2 - 1;
	uint8_t *binary = token->string;

	for (size_t iter = 0; iter < binlen; ++iter)
		binary[iter] = scanOctet(string + iter * 2);

	if (sqlite3_bind_blob(param->stmt, param->index,
		binary, binlen, SQLITE_STATIC))
		die("Failed to bind blob: ",
			sqlite3_errmsg(db), NULL);
}

static void bindZero(const struct param *restrict param,
	struct token *restrict token) {

	if (token->length < 2
		|| token->string[token->length - 1] != ']')
		die("No closing bracket at end of zero blob", NULL);

	/* Clip brackets */
	const char *string = token->string + 1;
	size_t length = token->length - 2;

	char *end;
	unsigned long binlen = strtoul(string, &end, 0);
	if (end != string + length)
		die("Invalid zero blob length “",
			string, "”", NULL);
	else if (binlen > ~0)
		die("Zero blob length ", string,
			" out of range", NULL);

	if (sqlite3_bind_zeroblob(param->stmt, param->index, binlen))
		die("Failed to bind zero blob: ",
			sqlite3_errmsg(db), NULL);
}

static void bindNumeric(const struct param *restrict param,
	struct token *restrict token) {
	long long int integer;
	double flpoint;

	char *end;

	errno = 0;
	integer = strtoll(token->string, &end, 0);
	if (end != token->string + token->length) {
		errno = 0;
		flpoint = strtod(token->string, &end);
		if (end != token->string + token->length)
			die("Invalid numeric value “",
				token->string, "”", NULL);
		else if (errno)
			die("Floating‐point value ", token->string,
				" out of range", NULL);
		else if (sqlite3_bind_double(param->stmt, param->index, flpoint))
			die("Failed to bind floating‐point value: ",
				sqlite3_errmsg(db), NULL);
	}
	else if (errno)
		die("Integer value out of range", NULL);
	else if (sqlite3_bind_int64(param->stmt, param->index, integer))
		die("Failed to bind integer value: ",
			sqlite3_errmsg(db), NULL);
}

static void bindKeyword(const struct param *restrict param,
	struct token *restrict token) {
	if (str_equal(token->string, "nil")) {
		if (sqlite3_bind_null(param->stmt, param->index))
			die("Failed to bind null value: ",
				sqlite3_errmsg(db), NULL);
	}
	else if (sqlite3_bind_text(param->stmt, param->index,
		token->string, token->length, SQLITE_STATIC))
		die("Failed to bind text value: ",
			sqlite3_errmsg(db), NULL);
}

static void bindToken(const struct param *restrict param,
	struct token *restrict token) {

	wchar_t wide;
	size_t width = extractWide(&wide, token->string, token->length);
	if (width == 0)
		die("Incomplete multi‐byte character at end of token", NULL);

	switch(wide) {
	case L'\'':
		bindQuoted(param, token);
		break;

	case L'{':
		bindBlob(param, token);
		break;

	case L'[':
		bindZero(param, token);
		break;

	default:
		if (iswdigit(wide)
			|| wide == radix
			|| wide == psign
			|| wide == nsign)
			bindNumeric(param, token);
		else if (iswalpha(wide))
			bindKeyword(param, token);
		else
			die("Unknown token “", token->string, "”", NULL);
		break;
	}
}

static void bindVector(sqlite3_stmt *restrict stmt, int offset,
	char *restrict vector[restrict], size_t number) {
	for (size_t iter = 0; iter < number; ++iter) {
		struct param param;
		param.stmt  = stmt;
		param.index = iter + offset + 1;

		struct token token;
		token.string  = vector[iter];
		token.length  = strlen(vector[iter]);

		bindToken(&param, &token);
	}
}

static bool bindStream(sqlite3_stmt *restrict stmt, int offset,
	stralloc *restrict sa, size_t number) {
	int ret;

	bool quote = false;
	size_t head = 0;
	size_t tail = 0;
	size_t iter = 0;

	stralloc_zero(sa);

	do {
		ret = buffer_getline_sa(buffer_0, sa);
		if (ret < 0)
			die("Failed to read line from standard input: ",
				strerror(errno), NULL);

		char  *string = sa->s;
		size_t length = sa->len;

		while (tail < length) {
			wchar_t wide;
			size_t width
				= extractWide(&wide, string + tail, length - tail);

			if (quote) {
				if (wide == L'\'')
					quote = false;
			}

			else if (wide == L'\'')
				quote = true;

			else if (iswcntrl(wide)
				|| iswspace(wide)) {
				if (head < tail) {
					string[tail] = '\0';
					struct param param;
					param.stmt  = stmt;
					param.index = ++iter + offset;

					struct token token;
					token.string = string + head;
					token.length = tail - head;

					bindToken(&param, &token);
				}

				head = tail + width;
			}

			tail += width;
		}
more:;
	} while (ret && quote);

	if (quote)
		die("Unmatched quotation mark in input line", NULL);

	while (iter < number) {
		if (sqlite3_bind_null(stmt, ++iter + offset))
			die("Failed to bind null value: ",
				sqlite3_errmsg(db), NULL);
	}

	return ret;
}

static void printInteger(int64_t value) {
	char buf[FMT_LONG];
	buffer_put(buffer_1, buf, fmt_longlong(buf, value));
}

static void printFloat(double value) {
	char buf[32];
	gcvt(value, 8, buf);
	buffer_puts(buffer_1, buf);
}

static void printText(const char *restrict value) {
	buffer_put(buffer_1, "'", 1);

	size_t head, tail;

	for (head = tail = 0; value[tail]; ++tail) {
		if (value[tail] == '\'') {
			buffer_put(buffer_1, value + head, tail - head);
			buffer_put(buffer_1, "'", 1);
			head = tail;
		}
	}

	if (head < tail)
		buffer_put(buffer_1, value + head, tail - head);
	buffer_put(buffer_1, "'", 1);
}

static void printNull() {
	buffer_put(buffer_1, "nil", 3);
}

static void printOctet(uint8_t octet) {
	char buf[2];
	buf[0] = fmt_tohex(octet >> 4);
	buf[1] = fmt_tohex(octet & 0xf);
	buffer_put(buffer_1, buf, sizeof buf);
}

static void printBlob(const uint8_t *restrict value, int length) {
	buffer_put(buffer_1, "{", 1);

	for (int iter = 0; iter < length; ++iter)
		printOctet(value[iter]);

	buffer_put(buffer_1, "}", 1);
}

static void printValue(sqlite3_stmt *restrict stmt, int col) {
	switch (sqlite3_column_type(stmt, col)) {
	case SQLITE_INTEGER:
		printInteger
			(sqlite3_column_int64(stmt, col));
		break;

	case SQLITE_FLOAT:
		printFloat
			(sqlite3_column_double(stmt, col));
		break;

	case SQLITE_TEXT:
		printText
			(sqlite3_column_text(stmt, col));
		break;

	case SQLITE_NULL:
		printNull();
		break;

	default:
		printBlob(
			sqlite3_column_blob(stmt, col),
			sqlite3_column_bytes(stmt, col));
		break;
	}
}

static void fetchColumns(sqlite3_stmt *restrict stmt) {
	int cols = sqlite3_column_count(stmt);
	for (int col = 0; col < cols; ++col) {
		if (col)
			buffer_put(buffer_1, "\t", 1);
		printValue(stmt, col);
	}

	buffer_putnlflush(buffer_1);
}

static void fetchRows(sqlite3_stmt *restrict stmt) {
	int step;

	do {
		step = sqlite3_step(stmt);
		if (step == SQLITE_DONE)
			break;
		else if (step != SQLITE_ROW)
			die("Failed to evaluate statement: ",
				sqlite3_errmsg(db), NULL);

		fetchColumns(stmt);
	} while (step == SQLITE_ROW);
}

static void sqlExec(const char *restrict query, const char *restrict error) {
	char *errmsg;

	if (sqlite3_exec(db, query, NULL, NULL, &errmsg))
		die(error, ": ", errmsg, NULL);
}

static sqlite3_stmt *sqlPrepare(const char *restrict query) {
	sqlite3_stmt *stmt;

	if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL))
		die("Failed to prepare statement: ",
			sqlite3_errmsg(db), NULL);
	return stmt;
}

static void sqlWalk(sqlite3_stmt *restrict stmt, size_t number) {
	int needed = sqlite3_bind_parameter_count(stmt);
	if (needed > number) {
		/* Bind additional arguments from standard input */
		stralloc sa = {0};

		while (bindStream(stmt, number, &sa, needed - number)) {
			fetchRows(stmt);
			if (sqlite3_reset(stmt))
				die("Unable to reset statement: ",
					sqlite3_errmsg(db), NULL);
		}

		stralloc_free(&sa);
	}
	else
		fetchRows(stmt);
}

static void sqlQuery(const char *restrict query,
	char *restrict vector[restrict], size_t number) {
	sqlite3_stmt *stmt = sqlPrepare(query);
	bindVector(stmt, 0, vector, number);

	sqlExec("BEGIN DEFERRED TRANSACTION;",
		"Unable to initiate transaction");

	sqlWalk(stmt, number);
	sqlite3_finalize(stmt);

	sqlExec("COMMIT TRANSACTION;",
		"Unable to commit transaction");
}

int main(int argc, char *argv[]) {
	locale();

	if (argc < 3)
		die(argv[0], " [data base] [query] [parameter]…\n\n"
			"Unbound parameters are read from standard input.", NULL);

	if (sqlite3_open(argv[1], &db))
		die("Failed to open data base “", argv[1], "”: ",
			sqlite3_errmsg(db), NULL);
	atexit(final);

	sqlQuery(argv[2], argv + 3, argc - 3);
	return EXIT_SUCCESS;
}
