module type S = sig type v end
module M = struct
  module F (X : S) = struct type 'a t = V : int t end
end
