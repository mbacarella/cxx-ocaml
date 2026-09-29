module P = struct
  module type A = sig type t = private < m : int; .. > end
  module F (X : A) = struct include X let y = 1 let z = 2 end
end
