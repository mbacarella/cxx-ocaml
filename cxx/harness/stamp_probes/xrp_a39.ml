module P = struct
  module type MyT = sig type t end
  module MyMap(X : MyT) = struct type u = X.t end
end
