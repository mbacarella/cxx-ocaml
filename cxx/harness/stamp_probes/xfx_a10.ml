module type S = sig type v end
module F : functor (X : S) -> sig type 'a t = V : int t end =
  functor (X : S) -> struct type 'a t = V : int t end
