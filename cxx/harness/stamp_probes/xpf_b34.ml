module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> sig type elt type t val iter : (elt ->
  unit) -> t -> unit end) (Elt : ORD) : sig module Z : SET end = struct
  module Z = struct type elt = int type t = int let iter _ _ = () end
end
