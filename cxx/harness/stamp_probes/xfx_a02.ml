module type S = sig type v end
module F (X : S) = struct type t = V : t end
