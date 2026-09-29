type a = A of int
and b = { x : a }
and c = C of b | D
exception E of int
type ext = ..
type ext += X of int
type k = K : 'a -> k
type k2 = K2 : 'a * 'a -> k2
type ('a, 'b) eq = Eq : ('a, 'a) eq
type r = { f : 'a. 'a -> 'a }
type n = { n : int } [@@unboxed]
type gr = G of { a : int } [@@unboxed]
