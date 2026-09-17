module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (M : sig module N : sig module F : functor (X : ORD) -> SET end type
  w end) = struct
end
