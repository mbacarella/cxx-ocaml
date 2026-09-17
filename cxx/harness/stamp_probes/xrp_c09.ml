module type MyT = sig
  type 'a wrap = My of 'a t
  and 'a t = private < map : 'b. ('a -> 'b) ->'b wrap; .. >
  val create : 'a list -> 'a t
end
module MyMap(X : MyT) = struct
  include X
end
module rec MyList : sig
  type 'a wrap = My of 'a t
  and 'a t = < map : 'b. ('a -> 'b) ->'b wrap >
  val create : 'a list -> 'a t
end = struct
  include MyMap(MyList)
  let create l = assert false
end
