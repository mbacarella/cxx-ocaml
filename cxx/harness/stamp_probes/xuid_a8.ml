module type S1 = sig type o = O of { x : int } end
let m1 = 0
module type S2 = sig type 'x n = N of { x: int } end
let m2 = 0
module type S3 = sig type a = A of { x : int } type b = B of { y : int } end
let m3 = 0
module type S4 = sig type a = A of { x : int } | B of { y : int } end
let m4 = 0
module type S5 = sig type a = A of { x : int } and b = B of { y : int } end
let m5 = 0
module M6 : sig type 'x p = P of { x: int } end =
  struct type 'x p = P of { x: int } end
let m6 = 0
module M7 = struct type 'x p = P of { x: int } end
let m7 = 0
module M8 =
  (struct type p = P of { x: int } end : sig type p = P of { x: int } end)
let m8 = 0
