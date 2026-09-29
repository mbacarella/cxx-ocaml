module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (M : sig module F : functor (X : ORD) -> SET end) = struct
  module type T = module type of M.F
end
