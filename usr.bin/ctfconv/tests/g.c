/*
 * Shapes GCC emits and clang does not: a global declared before its
 * definition gets a defining DIE with only DW_AT_specification and the
 * location; a zero-length array has an upper bound of -1 written in the
 * unsigned form of the target's size type (all ones).
 */
extern int gspec;
extern const char *gname;
struct zl {
	int n;
	int tail[0];
};
struct zl *zp;
int gspec = 5;
const char *gname = "spec";
int use(struct zl *z) { return z->n + gspec; }
