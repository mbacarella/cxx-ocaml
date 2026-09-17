module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = struct type 'a t = 'a X.t = Succ of 'a t end
module rec MyList : MyT = MyMap(MyList)
