module P = struct
  module type MyT = sig
    type 'a wrap = My of 'a t
    and 'a t = private < map : 'b. ('a -> 'b) ->'b wrap; .. >
    val create : 'a list -> 'a t
  end
  module MyMap(X : MyT) = struct
    include X
    let f : 'b. 'b -> 'b wrap = fun f -> My (create []) 
  end
end
