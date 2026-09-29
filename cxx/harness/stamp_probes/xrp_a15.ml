module P = struct
  module type MyT = sig type t val x : t end
  module MyMap(X : MyT) = struct let y = 1 end
end
