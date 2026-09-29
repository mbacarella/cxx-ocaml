module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> sig type t module M : sig type u end end)
  (Elt : ORD) = struct
  include F(Elt)
end
