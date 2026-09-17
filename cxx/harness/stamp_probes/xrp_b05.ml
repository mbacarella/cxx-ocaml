module type MyT = sig type t = A end
module MyMap(X : MyT) = struct include X end
module rec MyList : MyT = MyMap(MyList)
