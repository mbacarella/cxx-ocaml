module type S = sig type v end
module type T = sig type 'a t = V : int t end
module F (X : S) : T = struct type 'a t = V : int t end
