module type S = sig type v end
module type T = functor (X : S) -> sig type 'a t = V : int t end
module F : T = functor (X : S) -> struct type 'a t = V : int t end
