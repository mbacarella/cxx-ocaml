module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> sig type t = X.t module M : sig type u =
  X.t end end) (Elt : ORD) = struct
end
