module type MyT = sig type t val x : t end
module MyMap(X : MyT) = struct include X end
module rec MyList : MyT = MyMap(MyList)
