module type S = sig type v end
module M = struct type v end
module F (X : S) = struct type 'a t = V : int t end
module N : sig end = F (M)
