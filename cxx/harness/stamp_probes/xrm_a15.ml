module type ORD = sig type t val compare : t -> t -> int end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit end
type 'a tree = E | N of 'a tree * 'a * 'a tree
module Bootstrap2
  (MakeDiet : functor (X: ORD) -> SET with type t = X.t tree and type elt =
    X.t) =
struct
  module rec Elt : sig
    type t = I of int * int | D of int * Diet.t * int
    val compare : t -> t -> int
  end = struct
    type t = I of int * int | D of int * Diet.t * int
    let compare x1 x2 = 0
  end
  and Diet : SET with type t = Elt.t tree and type elt = Elt.t = MakeDiet(Elt)
end
