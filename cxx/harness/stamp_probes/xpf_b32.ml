module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : sig type t end) -> sig type elt type t val iter :
  (elt -> unit) -> t -> unit end) (Elt : ORD) = struct
end
