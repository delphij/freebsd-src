/*
 * Built without -g: no DWARF at all, so ctfconv must
 * leave the file untouched and exit 0.
 */
int fvar = 1;

int
ffn(int x)
{
	return (x + fvar);
}
