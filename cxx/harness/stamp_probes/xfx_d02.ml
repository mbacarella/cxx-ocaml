module type S = sig type v end
module F (X : S) = struct
  module G (Y : sig type w type w2 end) = struct type 'a u = W : int u end
end
