type 'x a = A of { x: int }
let m1 = 0
type b = B of { x: int }
let m2 = 0
type ('x, 'y) c = C of { x: int; y : 'x }
let m3 = 0
type d = D of { x: int } | E of { y : int }
let m4 = 0
type 'x e = E1 of { x: int } | E2 of { y : int }
let m5 = 0
type 'x f = F1 of { x: int } | F2 of int
let m6 = 0
type ('x, 'y, 'z) g = G of { x: int }
let m7 = 0
type h = H of { x: int } and 'a i = I of { y : int }
let m8 = 0
type 'x j = J of { x: int; mutable y : int }
let m9 = 0
type +'x k = K of { x: int }
let m10 = 0
type _ l = L of { x: int }
let m11 = 0
module type S = sig type 'x n = N of { x: int } type o = O of { x : int } end
let m12 = 0
module M : sig type 'x p = P of { x: int } end =
  struct type 'x p = P of { x: int } end
let m13 = 0
