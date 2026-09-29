module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module Elt : sig type t end = struct type t = int end
module B (F : functor (X : ORD) -> SET) = struct
  module rec Diet : SET = F(Elt)
end
