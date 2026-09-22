module D1 : sig exception E of { x : int } end =
  struct exception E of { x : int } end
let m1 = 0
type t = ..
module D2 : sig type t += A of { x : int } end =
  struct type t += A of { x : int } end
let m2 = 0
module D3 : sig type p = P of { x : int } end =
  struct type p = P of { x : int } let f = 1 end
let m3 = 0
module D4 : sig type p = P of { x : int } end = struct
  type p = P of { x : int }
  module M = struct type q = Q of { y : int } end
end
let m4 = 0
module D5 : sig type p = P of { x : int } end = struct
  type p = P of { x : int }
  module type S = sig type q = Q of { y : int } end
end
let m5 = 0
module D6 : sig type p = P of { x : int } end = struct
  type p = P of { x : int }
  let g = let module M = struct type q = Q of { y : int } end in 1
end
let m6 = 0
