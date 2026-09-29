module P = struct
  module type MyT = sig type t end
  module MyMap(X : MyT) = struct include X end
end
