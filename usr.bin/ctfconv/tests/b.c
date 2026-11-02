struct fwd { long l; double d; };        /* defines a.c's forward */
struct node { int v; struct node *next; }; /* CONFLICT: differs from a.c */
struct node bnode;                          /* used, so the DWARF carries it */
static int si = 7;                          /* same-name static, different file */
static struct fwd sf;
int g1(struct fwd *p) { return (int)p->l + si + (int)sf.d; }
static long f2(long y) { return y + 1; }   /* same-name static fn, different signature */
long g2(long z) { return f2(z); }
