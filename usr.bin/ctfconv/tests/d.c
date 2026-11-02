/* out-of-line instance of an inline function (DW_AT_abstract_origin), VLA, DW_AT_count arrays, atomic, _Complex float, long long */
static inline int h(int x) { return x * 3 + 1; }
int (*fp)(int) = h;                           /* forces concrete out-of-line h */
int use_h(int a, int b) { return h(a) + h(b); }
int vla(int n) { int v[n]; v[0] = n; return v[0] + (int)sizeof(v); }
_Atomic int at; _Complex float cf; long long ll; unsigned short us; unsigned u; long int li; short int shi; long unsigned int lui;
int arr3[2][3][4];
