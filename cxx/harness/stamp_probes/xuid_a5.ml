type t = ..
type t += A | B of int | C of { x : int; y : int }
type t += D = A
type m1 = int
exception F of { y : int }
type m2 = int
exception G of int
exception G2 = G
type m3 = int
type t += E : int -> t
type m4 = int
module M = struct type t += H | I of { z : int } end
type m5 = int
module type S = sig type t += J | K of { w : int } end
type m6 = int
