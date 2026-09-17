module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = X
module rec MyList : MyT = MyMap(MyList)
and B : sig val x : int end = struct let x = 1 end
