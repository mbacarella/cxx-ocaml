module type MyT = sig type 'a t = Succ of 'a t end
module MyMap(X : MyT) = X
module M = MyMap(struct type 'a t = Succ of 'a t end)
