module type S = sig
  type t
  val f : t -> int
end
module M : S = struct
  type t = int
  let f x = x
end
include M
let b = f
