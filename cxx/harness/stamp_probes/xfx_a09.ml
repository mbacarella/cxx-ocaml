module type S = sig type v end
module F (X : S) (Y : S) = struct type 'a t = V : int t end
