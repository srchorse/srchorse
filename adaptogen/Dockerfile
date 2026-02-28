FROM node:20-alpine

WORKDIR /app

COPY package*.json ./
RUN apk add --no-cache bash
RUN npm install --omit=dev

COPY . .

EXPOSE 245

CMD ["npm", "start"]
