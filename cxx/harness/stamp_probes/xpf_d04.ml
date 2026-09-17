module type ORD = sig
  type t
  val compare : t -> t -> int
end

module type SET = sig
  type elt
  type t
  val iter : (elt -> unit) -> t -> unit
end

type 'a tree = E | N of 'a tree * 'a * 'a tree

module Bootstrap2
  (MakeDiet : functor (X: ORD) -> SET)
  =
struct
  module rec Elt : sig
    type t = I of int * int | D of int * int * int
    val compare : t -> t -> int
  end =
  struct
    type t = I of int * int | D of int * int * int
    let compare x1 x2 = 0
  end
  and Diet : SET = MakeDiet(Elt)
end
