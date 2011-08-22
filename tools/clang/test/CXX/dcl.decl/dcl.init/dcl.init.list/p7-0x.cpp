// RUN: %clang_cc1 -fsyntax-only -std=c++0x -triple x86_64-apple-macosx10.6.7 -verify %s

// Verify that narrowing conversions in initializer lists cause errors in C++0x
// mode.

void std_example() {
  int x = 999;  // x is not a constant expression
  const int y = 999;
  const int z = 99;
  char c1 = x;  // OK, though it might narrow (in this case, it does narrow)
  char c2{x};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  char c3{y};  // expected-error {{ cannot be narrowed }} expected-note {{override}} expected-warning {{changes value}}
  char c4{z};  // OK: no narrowing needed
  unsigned char uc1 = {5};  // OK: no narrowing needed
  unsigned char uc2 = {-1};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  unsigned int ui1 = {-1};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  signed int si1 =
    { (unsigned int)-1 };  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  int ii = {2.0};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  float f1 { x };  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  float f2 { 7 };  // OK: 7 can be exactly represented as a float
  int f(int);
  int a[] =
    { 2, f(2), f(2.0) };  // OK: the double-to-int conversion is not at the top level
}

// Test each rule individually.

template<typename T>
struct Agg {
  T t;
};

// C++0x [dcl.init.list]p7: A narrowing conversion is an implicit conversion
//
// * from a floating-point type to an integer type, or

void float_to_int() {
  Agg<char> a1 = {1.0F};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<char> a2 = {1.0};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<char> a3 = {1.0L};  // expected-error {{ cannot be narrowed }} expected-note {{override}}

  float f = 1.0;
  double d = 1.0;
  long double ld = 1.0;
  Agg<char> a4 = {f};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<char> a5 = {d};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<char> a6 = {ld};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
}

// * from long double to double or float, or from double to float, except where
//   the source is a constant expression and the actual value after conversion
//   is within the range of values that can be represented (even if it cannot be
//   represented exactly), or

void shrink_float() {
  // These aren't constant expressions.
  float f = 1.0;
  double d = 1.0;
  long double ld = 1.0;

  // Variables.
  Agg<float> f1 = {f};  // OK  (no-op)
  Agg<float> f2 = {d};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<float> f3 = {ld};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  // Exact constants.
  Agg<float> f4 = {1.0};  // OK  (double constant represented exactly)
  Agg<float> f5 = {1.0L};  // OK  (long double constant represented exactly)
  // Inexact but in-range constants.
  Agg<float> f6 = {0.1};  // OK (double constant in range but rounded)
  Agg<float> f7 = {0.1L};  // OK (long double constant in range but rounded)
  // Out of range constants.
  Agg<float> f8 = {1E50};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<float> f9 = {1E50L};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  // More complex constant expression.
  constexpr long double e40 = 1E40L, e30 = 1E30L, e39 = 1E39L;
  Agg<float> f10 = {e40 - 5 * e39 + e30 - 5 * e39};  // OK

  // Variables.
  Agg<double> d1 = {f};  // OK  (widening)
  Agg<double> d2 = {d};  // OK  (no-op)
  Agg<double> d3 = {ld};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  // Exact constant.
  Agg<double> d4 = {1.0L};  // OK  (long double constant represented exactly)
  // Inexact but in-range constant.
  Agg<double> d5 = {0.1L};  // OK (long double constant in range but rounded)
  // Out of range constant.
  Agg<double> d6 = {1E315L};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  // More complex constant expression.
  constexpr long double e315 = 1E315L, e305 = 1E305L, e314 = 1E314L;
  Agg<double> d7 = {e315 - 5 * e314 + e305 - 5 * e314};  // OK
}

// * from an integer type or unscoped enumeration type to a floating-point type,
//   except where the source is a constant expression and the actual value after
//   conversion will fit into the target type and will produce the original
//   value when converted back to the original type, or
void int_to_float() {
  // Not a constant expression.
  char c = 1;

  // Variables.  Yes, even though all char's will fit into any floating type.
  Agg<float> f1 = {c};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<double> f2 = {c};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<long double> f3 = {c};  // expected-error {{ cannot be narrowed }} expected-note {{override}}

  // Constants.
  Agg<float> f4 = {12345678};  // OK (exactly fits in a float)
  Agg<float> f5 = {123456789};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
}

// * from an integer type or unscoped enumeration type to an integer type that
//   cannot represent all the values of the original type, except where the
//   source is a constant expression and the actual value after conversion will
//   fit into the target type and will produce the original value when converted
//   back to the original type.
void shrink_int() {
  // Not a constant expression.
  short s = 1;
  unsigned short us = 1;
  Agg<char> c1 = {s};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<unsigned short> s1 = {s};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<short> s2 = {us};  // expected-error {{ cannot be narrowed }} expected-note {{override}}

  // "that cannot represent all the values of the original type" means that the
  // validity of the program depends on the relative sizes of integral types.
  // This test compiles with -m64, so sizeof(int)<sizeof(long)==sizeof(long
  // long).
  long l1 = 1;
  Agg<int> i1 = {l1};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  long long ll = 1;
  Agg<long> l2 = {ll};  // OK

  // Constants.
  Agg<char> c2 = {127};  // OK
  Agg<char> c3 = {300};  // expected-error {{ cannot be narrowed }} expected-note {{override}} expected-warning {{changes value}}

  Agg<int> i2 = {0x7FFFFFFFU};  // OK
  Agg<int> i3 = {0x80000000U};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<unsigned int> i4 = {-0x80000000L};  // expected-error {{ cannot be narrowed }} expected-note {{override}}

  // Bool is also an integer type, but conversions to it are a different AST
  // node.
  Agg<bool> b1 = {0};  // OK
  Agg<bool> b2 = {1};  // OK
  Agg<bool> b3 = {-1};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
}

// Be sure that type- and value-dependent expressions in templates get the error
// too.

template<int I, typename T>
void maybe_shrink_int(T t) {
  Agg<short> s1 = {t};  // expected-error {{ cannot be narrowed }} expected-note {{override}}
  Agg<short> s2 = {I};  // expected-error {{ cannot be narrowed }} expected-note {{override}} expected-warning {{changes value}}
  Agg<T> t2 = {700};  // expected-error {{ cannot be narrowed }} expected-note {{override}} expected-warning {{changes value}}
}

void test_template() {
  maybe_shrink_int<15>((int)3);  // expected-note {{in instantiation}}
  maybe_shrink_int<70000>((char)3);  // expected-note {{in instantiation}}
}