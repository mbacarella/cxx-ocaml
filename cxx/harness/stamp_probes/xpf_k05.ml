module type ORD = sig type t end
module type SET = sig type t end
module rec B : functor (F : functor (X : ORD) -> SET) -> sig end = functor (F
  : functor (X : ORD) -> SET) -> struct end
