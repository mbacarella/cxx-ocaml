module P = struct
  module type A = sig type t = private < m : int; .. > end
  module F (X : A) = struct include X exception E end
end
