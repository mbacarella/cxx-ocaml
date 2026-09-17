module type S = sig type v end
module F (X : S) (Y : S) (Z : S) = struct type 'a u = W : int u end
