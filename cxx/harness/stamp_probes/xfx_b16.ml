module type S = sig type v end
module F (_ : S) = struct type 'a t = V : int t end
