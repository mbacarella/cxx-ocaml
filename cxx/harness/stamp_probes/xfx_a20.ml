module type S = sig type v end
module F () = struct type 'a t = V : int t end
