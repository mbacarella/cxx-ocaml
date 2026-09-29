module P = struct
  module type A = sig type t end
  module MyMap(X : A) = struct include (X : A) end
end
