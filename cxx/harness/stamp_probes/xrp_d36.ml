module P = struct
  module type MyT = sig
    type 'a wrap = My of 'a t
    and 'a t = private < map : 'b. ('a -> 'b) ->'b wrap; .. >
    val create : 'a list -> 'a t
  end
  module MyMap(X : MyT) = struct
    include X
    class ['a] c l = object method map : 'b. 'b -> 'b = fun f -> f end
  end
end
