module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = struct include X let x = 1 end
module rec MyList : MyT = MyMap(MyList)
