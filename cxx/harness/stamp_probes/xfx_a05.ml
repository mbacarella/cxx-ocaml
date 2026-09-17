module type S = sig type v end
module F (X : S) = struct type 'a t = { x : 'a t option } end
