module type S0 = sig type z1 type z2 end
module M0 = struct type z1 type z2 end
module type S0' = sig include S0 type additional end
module G (X : S0) = struct type z1 type z2 type additional end
module Gn (_ : S0) = struct type z1 type z2 type additional end
module Gs (X : S0) = struct type z1 = X.z1 type additional end
module Gsn (_ : S0) = struct type z1 type additional end
