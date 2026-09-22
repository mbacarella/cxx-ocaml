type f = float
type t1 = { x : f }
type f2 = float and t2 = { y : f2 }
type u = U of float [@@unboxed]
type t3 = { z : u }
type t4 = { a : float; b : float }
type t5 = { a : float; b : int }
type t6 = { mutable c : float }
type t7 = { d : float } [@@unboxed]
type t8 = { e : Stdlib.Float.t }
type t9 = { f : Float.t }
type g = { g : float } [@@unboxed]
type t10 = { h : g }
type t11 = { i : Complex.t }
type nonrec f3 = float
type nonrec t12 = { j : f3 }
type 'a p = float
type t13 = { k : int p }
type t14 = { mutable l : float [@atomic] }
module M = struct type m = float end
type t15 = { n : M.m }
type ('a, 'b) t16 = { o : float; p : float }
type pf = private float
type t18 = { r : pf }
