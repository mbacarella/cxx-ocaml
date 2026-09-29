module P = struct
  module type A = sig type t = private < m : int; .. > val v : t end
  module F (X : A) = struct include X end
end
