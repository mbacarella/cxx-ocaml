module type MyT = sig type t = Succ of t end
module MyMap(X : MyT) = struct include X end
module rec MyList : MyT = MyMap(MyList)
