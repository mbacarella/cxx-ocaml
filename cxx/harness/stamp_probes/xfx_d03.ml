module type S = sig type v end
module F (X : S) = struct
  module G (Y : S) (Z : S) = struct type 'a u = W : int u end
end
