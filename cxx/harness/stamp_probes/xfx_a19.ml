module type S = sig type v end
module F (X : S) = struct
  module G (Y : S) = struct type 'a t = V : int t end
end
