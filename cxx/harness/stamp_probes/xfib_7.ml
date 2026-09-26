module type S0 = sig type key end
module A = struct
  module M0 = struct type t let v = 3 end
  module F (X : S0) = M0
  module G (M0 : S0) = M0
end
