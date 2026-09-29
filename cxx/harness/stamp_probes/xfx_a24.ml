module type S = sig type v end
module F (X : S) : sig type 'a t = V : int t end =
  struct type 'a t = V : int t end
