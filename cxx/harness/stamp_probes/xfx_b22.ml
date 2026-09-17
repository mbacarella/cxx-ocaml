module type S = sig type v end
module F (X : S) = struct
  type 'a t = V : int t
  module G (Y : S) = struct type 'a u = W : int u end
end
