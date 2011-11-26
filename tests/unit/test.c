#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "alloc.h"

char *file_read_string(FILE *in)
{
fpos_t pos;
long size;
char *buffer;

assert_int_equal(fgetpos(in, &pos), 0);

assert_int_equal(fseek(in, 0, SEEK_END), 0);
size = ftell(in);
assert_true(size >= 0);

assert_int_equal(fseek(in, 0, SEEK_SET), 0);

buffer = xcalloc(size + 1L, sizeof(char));
assert_int_equal(fread(buffer, 1, size, in), size);

assert_int_equal(fsetpos(in, &pos), 0);

return buffer;
}

void assert_file_equal(FILE *a, FILE *b)
{
char *a_buffer = file_read_string(a);
char *b_buffer = file_read_string(b);

if (strcmp(a_buffer, b_buffer) != 0)
   {
   printf("\n=====\n%s != \n=====\n%s\n", a_buffer, b_buffer);
   fail();
   }

free(a_buffer);
free(b_buffer);
}
