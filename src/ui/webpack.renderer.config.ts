import path from 'path';

import type { Configuration } from 'webpack';

import { rules } from './webpack.rules';
import { plugins } from './webpack.plugins';

// eslint-disable-next-line @typescript-eslint/no-var-requires -- CommonJS plugin export
const tailwindPostcss = require('@tailwindcss/postcss')({
  base: path.resolve(__dirname),
});

rules.push({
  test: /\.css$/,
  use: [
    { loader: 'style-loader' },
    {
      loader: 'css-loader',
      options: { importLoaders: 1 },
    },
    {
      loader: 'postcss-loader',
      options: {
        postcssOptions: {
          plugins: [tailwindPostcss],
        },
      },
    },
  ],
});

export const rendererConfig: Configuration = {
  module: {
    rules,
  },
  plugins,
  resolve: {
    extensions: ['.js', '.ts', '.jsx', '.tsx', '.css'],
  },
};
